#include "core/deb_analyzer.hpp"

#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/dependency_parser.hpp"
#include "core/path_safety.hpp"
#include "core/script_evidence.hpp"

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTemporaryFile>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <memory>

namespace pacsmith {
namespace {

struct ArchiveDeleter {
    void operator()(archive *value) const {
        if (value != nullptr) archive_read_free(value);
    }
};
using ArchivePointer = std::unique_ptr<archive, ArchiveDeleter>;

QString archiveMessage(archive *reader, const QString &fallback) {
    const char *message = archive_error_string(reader);
    return message != nullptr ? QString::fromUtf8(message) : fallback;
}

QString entryPath(archive_entry *entry) {
    const char *path = archive_entry_pathname_utf8(entry);
    if (path == nullptr) path = archive_entry_pathname(entry);
    return path != nullptr ? QString::fromUtf8(path) : QString{};
}

bool copyCurrentEntry(archive *reader, QTemporaryFile &file, QString &error) {
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const auto read = archive_read_data(reader, buffer.data(), buffer.size());
        if (read == 0) return true;
        if (read < 0) {
            error = archiveMessage(reader, QStringLiteral("Could not read DEB member"));
            return false;
        }
        const auto length = static_cast<qint64>(read);
        if (file.write(buffer.data(), length) != length) {
            error = file.errorString();
            return false;
        }
    }
}

QByteArray readCurrentEntry(archive *reader, const qsizetype maximumBytes, QString &error) {
    QByteArray result;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        const auto read = archive_read_data(reader, buffer.data(), buffer.size());
        if (read == 0) return result;
        if (read < 0) {
            error = archiveMessage(reader, QStringLiteral("Could not read archive member"));
            return {};
        }
        const auto length = static_cast<qsizetype>(read);
        if (result.size() > maximumBytes - length) {
            error = QStringLiteral("Archive metadata member exceeds the safety limit");
            return {};
        }
        result.append(buffer.data(), length);
    }
}

struct EntryInspection {
    QByteArray captured;
    QString sha256;
    bool truncated{false};
};

EntryInspection inspectCurrentEntry(archive *reader, const qsizetype captureLimit, QString &error) {
    EntryInspection result;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const auto read = archive_read_data(reader, buffer.data(), buffer.size());
        if (read == 0) break;
        if (read < 0) {
            error = archiveMessage(reader, QStringLiteral("Could not inspect archive member"));
            return {};
        }
        const auto length = static_cast<qsizetype>(read);
        hash.addData(QByteArrayView(buffer.data(), length));
        const auto available = captureLimit - result.captured.size();
        if (available > 0) result.captured.append(buffer.data(), std::min(length, available));
        if (length > available) result.truncated = true;
    }
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    return result;
}

ArchivePointer openTar(const QString &path, QString &error) {
    ArchivePointer reader(archive_read_new());
    if (!reader) {
        error = QStringLiteral("Could not allocate archive reader");
        return {};
    }
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_tar(reader.get());
    const auto encoded = QFile::encodeName(path);
    if (archive_read_open_filename(reader.get(), encoded.constData(), 64 * 1024) != ARCHIVE_OK) {
        error = archiveMessage(reader.get(), QStringLiteral("Could not open nested tar archive"));
        return {};
    }
    return reader;
}

QString entryType(archive_entry *entry) {
    switch (archive_entry_filetype(entry)) {
    case AE_IFREG: return QStringLiteral("file");
    case AE_IFDIR: return QStringLiteral("directory");
    case AE_IFLNK: return QStringLiteral("symlink");
    case AE_IFCHR: return QStringLiteral("character device");
    case AE_IFBLK: return QStringLiteral("block device");
    case AE_IFIFO: return QStringLiteral("fifo");
    case AE_IFSOCK: return QStringLiteral("socket");
    default: return QStringLiteral("other");
    }
}

bool shouldInspectContents(const QString &path, const qint64 size) {
    if (size < 0 || size > 1024 * 1024) return false;
    return path.startsWith(QStringLiteral("etc/apt/")) ||
           path.endsWith(QStringLiteral(".sources")) ||
           path.endsWith(QStringLiteral(".list"));
}

bool isDesktopEntry(const QString &path, const qint64 size) {
    return size >= 0 && size <= 256 * 1024 &&
           path.startsWith(QStringLiteral("usr/share/applications/")) &&
           path.endsWith(QStringLiteral(".desktop"));
}

bool isDirectOptApplication(const QString &path) {
    const auto components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return components.size() == 3 && components.first() == QStringLiteral("opt");
}

bool likelyUserCommand(const QString &path) {
    const auto name = QFileInfo(path).fileName().toLower();
    if (name.isEmpty() || name.endsWith(QStringLiteral(".so")) ||
        name.contains(QStringLiteral(".so.")) || name.contains(QStringLiteral("debug")) ||
        name.contains(QStringLiteral("crashpad")) || name.contains(QStringLiteral("sandbox"))) {
        return false;
    }
    return isDirectOptApplication(path) || path.startsWith(QStringLiteral("usr/bin/")) ||
           path.startsWith(QStringLiteral("usr/local/bin/")) ||
           path.startsWith(QStringLiteral("bin/"));
}

bool isIconCandidate(const QString &path, const qint64 size) {
    if (size <= 0 || size > 4 * 1024 * 1024) return false;
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("svg") &&
        suffix != QStringLiteral("xpm")) {
        return false;
    }
    return (path.startsWith(QStringLiteral("usr/share/icons/")) &&
            path.contains(QStringLiteral("/apps/"))) ||
           path.startsWith(QStringLiteral("usr/share/pixmaps/"));
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
               static_cast<quint64>(width) * static_cast<quint64>(height) <= 4U * 1024U * 1024U;
    }
    if (suffix == QStringLiteral("xpm")) {
        return contents.size() <= 1024 * 1024 &&
               contents.left(256).contains("/* XPM */");
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
        references.contains(stem)) {
        score += 100000;
    }
    if (candidate.path.startsWith(QStringLiteral("usr/share/icons/"))) score += 10000;
    const auto suffix = info.suffix().toLower();
    score += suffix == QStringLiteral("png") ? 3000
           : suffix == QStringLiteral("svg") ? 2000 : 1000;
    static const QRegularExpression dimensions(QStringLiteral(R"(/(\d+)x(\d+)/apps/)"));
    const auto match = dimensions.match(candidate.path);
    if (match.hasMatch()) {
        const auto width = match.captured(1).toInt();
        const auto height = match.captured(2).toInt();
        score += std::min(std::min(width, height), 512);
    }
    if (!packageName.isEmpty() && stem.contains(packageName, Qt::CaseInsensitive)) score += 500;
    return score;
}

} // namespace

std::optional<DebAnalysis> DebAnalyzer::analyze(const std::filesystem::path &path,
                                                AnalysisError &error,
                                                const ImportProgressCallback &progress) const {
    if (progress) progress({ImportStage::ReadingDebContainer, 0});
    const auto filePath = QString::fromStdString(path.string());
    if (!QFileInfo::exists(filePath) || !QFileInfo(filePath).isFile()) {
        error.message = QStringLiteral("DEB file does not exist or is not a regular file: %1").arg(filePath);
        return std::nullopt;
    }

    ArchivePointer outer(archive_read_new());
    if (!outer) {
        error.message = QStringLiteral("Could not allocate archive reader");
        return std::nullopt;
    }
    archive_read_support_filter_all(outer.get());
    archive_read_support_format_ar(outer.get());
    const auto encodedPath = QFile::encodeName(filePath);
    if (archive_read_open_filename(outer.get(), encodedPath.constData(), 64 * 1024) != ARCHIVE_OK) {
        error.message = archiveMessage(outer.get(), QStringLiteral("Could not open DEB archive"));
        return std::nullopt;
    }

    QTemporaryFile controlArchive;
    QTemporaryFile dataArchive;
    if (!controlArchive.open() || !dataArchive.open()) {
        error.message = QStringLiteral("Could not create temporary archive files");
        return std::nullopt;
    }
    bool foundControl = false;
    bool foundData = false;
    bool validMarker = false;
    archive_entry *entry = nullptr;
    int nextHeaderStatus = ARCHIVE_OK;
    while ((nextHeaderStatus = archive_read_next_header(outer.get(), &entry)) == ARCHIVE_OK) {
        QString name = entryPath(entry);
        if (name.endsWith(QLatin1Char('/'))) name.chop(1);
        if (name == QStringLiteral("debian-binary")) {
            QString readError;
            const auto marker = readCurrentEntry(outer.get(), 64, readError).trimmed();
            if (!readError.isEmpty()) {
                error.message = readError;
                return std::nullopt;
            }
            validMarker = marker == QByteArrayLiteral("2.0");
        } else if (name.startsWith(QStringLiteral("control.tar")) && !foundControl) {
            QString copyError;
            if (!copyCurrentEntry(outer.get(), controlArchive, copyError)) {
                error.message = copyError;
                return std::nullopt;
            }
            foundControl = true;
        } else if (name.startsWith(QStringLiteral("data.tar")) && !foundData) {
            QString copyError;
            if (!copyCurrentEntry(outer.get(), dataArchive, copyError)) {
                error.message = copyError;
                return std::nullopt;
            }
            foundData = true;
        }
    }
    if (nextHeaderStatus != ARCHIVE_EOF) {
        error.message = archiveMessage(outer.get(), QStringLiteral("Could not finish reading the DEB archive"));
        return std::nullopt;
    }
    if (!validMarker || !foundControl || !foundData) {
        error.message = QStringLiteral("Not a supported Debian binary package (missing debian-binary, control.tar.*, or data.tar.*)");
        return std::nullopt;
    }
    controlArchive.flush();
    dataArchive.flush();

    DebAnalysis result;
    QString nestedError;
    if (progress) progress({ImportStage::ReadingControlArchive, 0});
    auto control = openTar(controlArchive.fileName(), nestedError);
    if (!control) {
        error.message = QStringLiteral("Could not read control archive: %1").arg(nestedError);
        return std::nullopt;
    }
    const QSet<QString> scriptNames{QStringLiteral("preinst"), QStringLiteral("postinst"),
                                    QStringLiteral("prerm"), QStringLiteral("postrm"),
                                    QStringLiteral("config")};
    QByteArray controlData;
    while ((nextHeaderStatus = archive_read_next_header(control.get(), &entry)) == ARCHIVE_OK) {
        const auto safePath = PathSafety::normalizedArchivePath(entryPath(entry));
        if (!safePath) {
            error.message = QStringLiteral("Unsafe path in control archive: %1").arg(entryPath(entry));
            return std::nullopt;
        }
        const auto name = safePath->section(QLatin1Char('/'), -1);
        if (archive_entry_filetype(entry) != AE_IFREG) continue;
        if (name == QStringLiteral("control") || scriptNames.contains(name)) {
            QString readError;
            const auto contents = readCurrentEntry(control.get(), 16 * 1024 * 1024, readError);
            if (!readError.isEmpty()) {
                error.message = readError;
                return std::nullopt;
            }
            if (name == QStringLiteral("control")) {
                controlData = contents;
            } else {
                result.maintainerScripts.append({name, QString::fromUtf8(contents), {}});
            }
        }
    }
    if (nextHeaderStatus != ARCHIVE_EOF) {
        error.message = archiveMessage(control.get(), QStringLiteral("Could not finish reading the control archive"));
        return std::nullopt;
    }
    if (controlData.isEmpty()) {
        error.message = QStringLiteral("The DEB control archive has no control metadata file");
        return std::nullopt;
    }
    result.metadata = ControlParser::parsePackage(QByteArrayView(controlData));
    if (result.metadata.package.isEmpty() || result.metadata.version.isEmpty()) {
        error.message = QStringLiteral("The DEB control metadata is missing Package or Version");
        return std::nullopt;
    }
    QString declarations = result.metadata.preDepends;
    if (!declarations.isEmpty() && !result.metadata.depends.isEmpty()) declarations.append(QStringLiteral(", "));
    declarations.append(result.metadata.depends);
    result.dependencies = DependencyParser::parse(declarations);
    static_cast<void>(DependencyParser::applyVerifiedMappings(
        result.dependencies, DependencyParser::loadVerifiedMappings()));

    QSet<QString> candidates;
    const auto scriptEvidence = ScriptEvidenceAnalyzer::analyze(result.maintainerScripts);
    result.scriptFindings = scriptEvidence.findings;
    result.signingKeys = scriptEvidence.signingKeys;
    result.aptCandidates = scriptEvidence.aptCandidates;
    for (const auto &script : result.maintainerScripts) {
        const auto urls = PathSafety::urlsFromText(script.contents);
        for (const auto &url : urls) candidates.insert(url);
    }

    auto data = openTar(dataArchive.fileName(), nestedError);
    if (!data) {
        error.message = QStringLiteral("Could not read data archive: %1").arg(nestedError);
        return std::nullopt;
    }
    QSet<QString> rulePaths;
    QSet<QString> desktopIconReferences;
    QList<IconCandidate> iconCandidates;
    qsizetype capturedIconBytes = 0;
    qsizetype payloadEntries = 0;
    if (progress) progress({ImportStage::ReadingPayloadArchive, payloadEntries});
    while ((nextHeaderStatus = archive_read_next_header(data.get(), &entry)) == ARCHIVE_OK) {
        const auto originalPath = entryPath(entry);
        const auto safePath = PathSafety::normalizedArchivePath(originalPath);
        if (!safePath) {
            error.message = QStringLiteral("Unsafe path in data archive: %1").arg(originalPath);
            return std::nullopt;
        }
        if (safePath->isEmpty()) continue;
        PayloadEntry payloadEntry;
        payloadEntry.path = *safePath;
        payloadEntry.type = entryType(entry);
        const auto rawSize = archive_entry_size(entry);
        payloadEntry.size = rawSize >= 0 ? static_cast<qint64>(rawSize) : 0;
        const char *symlink = archive_entry_symlink_utf8(entry);
        if (symlink == nullptr) symlink = archive_entry_symlink(entry);
        if (symlink != nullptr) {
            payloadEntry.symlinkTarget = QString::fromUtf8(symlink);
            if (!PathSafety::safeSymlinkTarget(*safePath, payloadEntry.symlinkTarget)) {
                error.message = QStringLiteral("Unsafe symlink in data archive: %1 -> %2")
                                    .arg(*safePath, payloadEntry.symlinkTarget);
                return std::nullopt;
            }
        }
        const char *hardlink = archive_entry_hardlink_utf8(entry);
        if (hardlink == nullptr) hardlink = archive_entry_hardlink(entry);
        if (hardlink != nullptr && !PathSafety::normalizedArchivePath(QString::fromUtf8(hardlink))) {
            error.message = QStringLiteral("Unsafe hardlink in data archive: %1").arg(*safePath);
            return std::nullopt;
        }
        const auto fileType = archive_entry_filetype(entry);
        payloadEntry.reviewReason = PathSafety::reviewReason(*safePath);
        payloadEntry.requiresReview = !payloadEntry.reviewReason.isEmpty() && fileType != AE_IFDIR;
        const bool specialEntry = fileType == AE_IFCHR || fileType == AE_IFBLK ||
                                  fileType == AE_IFIFO || fileType == AE_IFSOCK;
        const auto permissions = archive_entry_perm(entry);
        if ((permissions & 06000) != 0) {
            if (!payloadEntry.reviewReason.isEmpty()) payloadEntry.reviewReason += QStringLiteral("; ");
            payloadEntry.reviewReason += QStringLiteral("Set-user-ID or set-group-ID permission requires review");
            payloadEntry.requiresReview = true;
        }
        if (specialEntry) {
            if (!payloadEntry.reviewReason.isEmpty()) payloadEntry.reviewReason += QStringLiteral("; ");
            payloadEntry.reviewReason += QStringLiteral("Special filesystem entry is excluded by default");
            payloadEntry.requiresReview = true;
        }
        if ((fileType == AE_IFREG || fileType == AE_IFLNK) &&
            (permissions & 0111) != 0 && likelyUserCommand(*safePath)) {
            LauncherMapping launcher;
            launcher.enabled = !isDirectOptApplication(*safePath);
            launcher.sourcePath = *safePath;
            launcher.commandName = QFileInfo(*safePath).fileName();
            launcher.destination = QStringLiteral("/usr/bin/%1").arg(launcher.commandName);
            launcher.provenance.origin = ValueOrigin::Deterministic;
            launcher.provenance.rationale = launcher.enabled
                ? QStringLiteral("Command detected in the inspected Debian payload")
                : QStringLiteral(
                      "Direct /opt application executable detected; enable it explicitly to expose a command");
            result.installMapping.launchers.append(launcher);
            if (result.installMapping.binarySourcePath.isEmpty()) {
                result.installMapping.binarySourcePath = launcher.sourcePath;
                result.installMapping.binaryDestination = launcher.destination;
            }
        }
        if (PathSafety::isDebianSpecificPath(*safePath)) {
            const bool aptPath = *safePath == QStringLiteral("etc/apt") ||
                                 safePath->startsWith(QStringLiteral("etc/apt/"));
            const bool keyringPath = *safePath == QStringLiteral("usr/share/keyrings") ||
                                     safePath->startsWith(QStringLiteral("usr/share/keyrings/"));
            const bool lintianPath = *safePath == QStringLiteral("usr/share/lintian") ||
                                     safePath->startsWith(QStringLiteral("usr/share/lintian/"));
            const auto exclusionPath = aptPath ? QStringLiteral("etc/apt")
                                     : keyringPath ? QStringLiteral("usr/share/keyrings")
                                     : lintianPath ? QStringLiteral("usr/share/lintian") : *safePath;
            const bool shouldExclude = aptPath || keyringPath || lintianPath ||
                                       archive_entry_filetype(entry) != AE_IFDIR;
            if (shouldExclude && !rulePaths.contains(exclusionPath)) {
                result.payloadRules.append({exclusionPath, true, payloadEntry.reviewReason, false, {}});
                rulePaths.insert(exclusionPath);
            }
        }
        if (specialEntry && !rulePaths.contains(*safePath)) {
            result.payloadRules.append({*safePath, true,
                                        QStringLiteral("Special filesystem entry is excluded by default"), false, {}});
            rulePaths.insert(*safePath);
        }
        const bool desktopEntry = fileType == AE_IFREG &&
                                  isDesktopEntry(*safePath, payloadEntry.size);
        const bool iconCandidate = fileType == AE_IFREG &&
                                   isIconCandidate(*safePath, payloadEntry.size);
        if (fileType == AE_IFREG &&
            (payloadEntry.requiresReview || shouldInspectContents(*safePath, payloadEntry.size) ||
             desktopEntry || iconCandidate)) {
            QString readError;
            const auto captureLimit = iconCandidate ? static_cast<qsizetype>(4 * 1024 * 1024)
                                                    : static_cast<qsizetype>(1024 * 1024);
            const auto inspection = inspectCurrentEntry(data.get(), captureLimit, readError);
            if (!readError.isEmpty()) {
                error.message = readError;
                return std::nullopt;
            }
            payloadEntry.contentSha256 = inspection.sha256;
            payloadEntry.previewTruncated = inspection.truncated;
            if (!inspection.captured.contains('\0')) {
                QStringDecoder decoder(QStringDecoder::Utf8);
                const auto decoded = decoder.decode(inspection.captured);
                if (!decoder.hasError()) payloadEntry.textPreview = decoded;
            }
            if (!payloadEntry.textPreview.isEmpty() && !inspection.truncated) {
                const auto payloadEvidence = ScriptEvidenceAnalyzer::analyze(
                    {{QStringLiteral("payload/%1").arg(*safePath),
                      payloadEntry.textPreview, {}}});
                for (const auto &candidate : payloadEvidence.rpmCandidates) {
                    const auto duplicate = std::any_of(
                        result.rpmCandidates.cbegin(), result.rpmCandidates.cend(),
                        [&](const auto &existing) {
                            return existing.baseUrl == candidate.baseUrl &&
                                   existing.architecture == candidate.architecture;
                        });
                    if (!duplicate) result.rpmCandidates.append(candidate);
                }
                for (const auto &key : payloadEvidence.signingKeys) {
                    const auto duplicate = std::any_of(
                        result.signingKeys.cbegin(), result.signingKeys.cend(),
                        [&](const auto &existing) { return existing.contents == key.contents; });
                    if (!duplicate) result.signingKeys.append(key);
                }
                for (const auto &url : PathSafety::urlsFromText(payloadEntry.textPreview)) {
                    candidates.insert(url);
                }
            }
            if (shouldInspectContents(*safePath, payloadEntry.size) && !inspection.truncated) {
                const auto urls = PathSafety::urlsFromText(QString::fromUtf8(inspection.captured));
                for (const auto &url : urls) candidates.insert(url);
                result.aptCandidates.append(AptSourcesParser::parse(QByteArrayView(inspection.captured), *safePath));
            }
            if (desktopEntry && !inspection.truncated) {
                collectDesktopIconReferences(inspection.captured, desktopIconReferences);
                DesktopEntryConfiguration desktop;
                desktop.id = QFileInfo(*safePath).completeBaseName();
                desktop.sourcePath = *safePath;
                desktop.destination = QStringLiteral("/usr/share/applications/%1")
                                          .arg(QFileInfo(*safePath).fileName());
                desktop.contents = QString::fromUtf8(inspection.captured);
                desktop.sourceSha256 = inspection.sha256;
                desktop.originalContentsSha256 = inspection.sha256;
                desktop.provenance.origin = ValueOrigin::Deterministic;
                desktop.provenance.rationale = QStringLiteral(
                    "Desktop entry detected in the inspected Debian payload");
                result.installMapping.desktopEntries.append(desktop);
            }
            constexpr qsizetype totalIconLimit = 32 * 1024 * 1024;
            if (iconCandidate && !inspection.truncated && iconCandidates.size() < 128 &&
                inspection.captured.size() <= totalIconLimit - capturedIconBytes &&
                validIconContents(*safePath, inspection.captured)) {
                capturedIconBytes += inspection.captured.size();
                iconCandidates.append({*safePath, inspection.captured});
            }
            if (safePath->startsWith(QStringLiteral("usr/share/keyrings/")) &&
                !inspection.truncated && !inspection.captured.isEmpty()) {
                result.signingKeys.append({inspection.captured, *safePath, inspection.sha256});
            }
        }
        result.payload.append(payloadEntry);
        ++payloadEntries;
        if (progress && payloadEntries % 128 == 0) {
            progress({ImportStage::ReadingPayloadArchive, payloadEntries});
        }
    }
    if (nextHeaderStatus != ARCHIVE_EOF) {
        error.message = archiveMessage(data.get(), QStringLiteral("Could not finish reading the data archive"));
        return std::nullopt;
    }
    if (progress) progress({ImportStage::ReadingPayloadArchive, payloadEntries});
    // Some DEBs intentionally keep their application executable under /opt and
    // expose it from postinst using ln or update-alternatives. The script remains
    // untrusted data and is never run; this only recognizes an exact source and
    // /usr/bin destination already present in its text. Other /opt candidates stay
    // visible but disabled for explicit user review.
    static const QRegularExpression launcherOperation(
        QStringLiteral("(?:ln[\\s\\S]{0,80}-s|update-alternatives[\\s\\S]{0,80}--install)"));
    for (auto &launcher : result.installMapping.launchers) {
        if (launcher.enabled || !isDirectOptApplication(launcher.sourcePath)) continue;
        const auto source = QLatin1Char('/') + launcher.sourcePath;
        const auto destination = launcher.destination.isEmpty()
            ? QStringLiteral("/usr/bin/%1").arg(launcher.commandName)
            : launcher.destination;
        const auto declared = std::any_of(
            result.maintainerScripts.cbegin(), result.maintainerScripts.cend(),
            [&](const auto &script) {
                return script.contents.contains(source) &&
                       script.contents.contains(destination) &&
                       launcherOperation.match(script.contents).hasMatch();
            });
        if (!declared) continue;
        launcher.enabled = true;
        launcher.provenance.rationale = QStringLiteral(
            "The Debian maintainer script explicitly exposes this inspected /opt executable at %1")
                                            .arg(destination);
    }
    if (!iconCandidates.isEmpty()) {
        const auto selected = std::max_element(
            iconCandidates.cbegin(), iconCandidates.cend(), [&](const auto &left, const auto &right) {
                return iconScore(left, desktopIconReferences, result.metadata.package) <
                       iconScore(right, desktopIconReferences, result.metadata.package);
            });
        result.icon = ExtractedPackageIcon{selected->path, selected->contents};
        result.installMapping.icon.sourceKind = IconSourceKind::Payload;
        result.installMapping.icon.sourcePath = selected->path;
        result.installMapping.icon.sha256 = QString::fromLatin1(
            QCryptographicHash::hash(selected->contents, QCryptographicHash::Sha256).toHex());
        result.installMapping.icon.format = QFileInfo(selected->path).suffix().toLower();
        result.installMapping.icon.iconName = result.metadata.package;
        result.installMapping.icon.provenance.origin = ValueOrigin::Deterministic;
        result.installMapping.icon.provenance.rationale = QStringLiteral(
            "Best matching icon selected from inspected Debian payload and desktop references");
    }
    result.updateCandidates = QStringList(candidates.cbegin(), candidates.cend());
    result.updateCandidates.sort();
    return result;
}

} // namespace pacsmith
