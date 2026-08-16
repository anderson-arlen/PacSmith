#include "core/payload_inspector.hpp"

#include "core/path_safety.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
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

QString archiveError(archive *reader, const QString &fallback) {
    const auto *message = archive_error_string(reader);
    return message == nullptr ? fallback : QString::fromUtf8(message);
}

QString entryPath(archive_entry *entry) {
    const auto *path = archive_entry_pathname_utf8(entry);
    if (path == nullptr) path = archive_entry_pathname(entry);
    return path == nullptr ? QString{} : QString::fromUtf8(path);
}

bool copyEntry(archive *reader, QTemporaryFile &destination, QString &error) {
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const auto count = archive_read_data(reader, buffer.data(), buffer.size());
        if (count == 0) return true;
        if (count < 0) {
            error = archiveError(reader, QStringLiteral("Could not read DEB data archive"));
            return false;
        }
        if (destination.write(buffer.data(), static_cast<qint64>(count)) != static_cast<qint64>(count)) {
            error = destination.errorString();
            return false;
        }
    }
}

PayloadInspection finishInspection(QCryptographicHash &hash, const QByteArray &captured,
                                   const bool truncated) {
    PayloadInspection result;
    result.contentSha256 = QString::fromLatin1(hash.result().toHex());
    result.previewTruncated = truncated;
    result.binary = captured.contains('\0');
    if (!result.binary) {
        QStringDecoder decoder(QStringDecoder::Utf8);
        result.textPreview = decoder.decode(captured);
        if (decoder.hasError()) {
            result.textPreview.clear();
            result.binary = true;
        }
    }
    return result;
}

std::optional<PayloadInspection> inspectArchiveEntry(archive *reader, const QString &safeTarget,
                                                     QString *error) {
    constexpr qsizetype previewLimit = 1024 * 1024;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const auto path = PathSafety::normalizedArchivePath(entryPath(entry));
        if (!path || *path != safeTarget) {
            archive_read_data_skip(reader);
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG) {
            if (error != nullptr) *error = QStringLiteral("Selected payload entry is not a regular file");
            return std::nullopt;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray captured;
        bool truncated = false;
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            const auto count = archive_read_data(reader, buffer.data(), buffer.size());
            if (count == 0) break;
            if (count < 0) {
                if (error != nullptr) {
                    *error = archiveError(reader, QStringLiteral("Could not read payload file"));
                }
                return std::nullopt;
            }
            const auto length = static_cast<qsizetype>(count);
            hash.addData(QByteArrayView(buffer.data(), length));
            const auto available = previewLimit - captured.size();
            if (available > 0) captured.append(buffer.data(), std::min(available, length));
            if (length > available) truncated = true;
        }
        return finishInspection(hash, captured, truncated);
    }
    if (status != ARCHIVE_EOF && error != nullptr) {
        *error = archiveError(reader, QStringLiteral("Could not scan artifact payload"));
    } else if (error != nullptr) {
        *error = QStringLiteral("Payload file was not found in the artifact");
    }
    return std::nullopt;
}

std::optional<QByteArray> readArchiveEntryBytes(archive *reader, const QString &safeTarget,
                                                const qsizetype maximumBytes,
                                                QString *error) {
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const auto path = PathSafety::normalizedArchivePath(entryPath(entry));
        if (!path || *path != safeTarget) {
            archive_read_data_skip(reader);
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG) {
            if (error != nullptr) *error = QStringLiteral("Selected payload entry is not a regular file");
            return std::nullopt;
        }
        QByteArray result;
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            const auto count = archive_read_data(reader, buffer.data(), buffer.size());
            if (count == 0) return result;
            if (count < 0) {
                if (error != nullptr) *error = archiveError(reader, QStringLiteral("Could not read payload file"));
                return std::nullopt;
            }
            if (result.size() + count > maximumBytes) {
                if (error != nullptr) *error = QStringLiteral("Payload file exceeds the %1-byte selection limit").arg(maximumBytes);
                return std::nullopt;
            }
            result.append(buffer.data(), count);
        }
    }
    if (error != nullptr) {
        *error = status == ARCHIVE_EOF ? QStringLiteral("Payload file was not found in the artifact")
                                      : archiveError(reader, QStringLiteral("Could not scan artifact payload"));
    }
    return std::nullopt;
}

std::optional<qint64> squashfsOffset(QFile &file) {
    if (!file.seek(0)) return std::nullopt;
    QByteArray overlap;
    qint64 position = 0;
    constexpr qint64 limit = 128LL * 1024 * 1024;
    while (position < std::min(file.size(), limit)) {
        const auto bytes = file.read(std::min<qint64>(1024 * 1024, limit - position));
        if (bytes.isEmpty()) break;
        const auto combined = overlap + bytes;
        auto index = combined.indexOf(QByteArrayLiteral("hsqs"));
        while (index >= 0) {
            const auto candidate = position - overlap.size() + index;
            if (candidate >= 4096 && candidate % 4 == 0) return candidate;
            index = combined.indexOf(QByteArrayLiteral("hsqs"), index + 1);
        }
        overlap = combined.right(3);
        position += bytes.size();
    }
    return std::nullopt;
}

std::optional<PayloadInspection> inspectRawFile(QFile &file, QString *error) {
    if (!file.seek(0)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    constexpr qsizetype previewLimit = 1024 * 1024;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray captured;
    bool truncated = false;
    while (!file.atEnd()) {
        const auto bytes = file.read(64 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError) {
            if (error != nullptr) *error = file.errorString();
            return std::nullopt;
        }
        hash.addData(QByteArrayView(bytes));
        const auto available = previewLimit - captured.size();
        if (available > 0) captured.append(bytes.first(available));
        if (bytes.size() > available) truncated = true;
    }
    return finishInspection(hash, captured, truncated);
}

} // namespace

std::optional<PayloadInspection> PayloadInspector::inspectFile(
    const std::filesystem::path &sourcePath, const QString &payloadPath, QString *error) {
    const auto safeTarget = PathSafety::normalizedArchivePath(payloadPath);
    if (!safeTarget || safeTarget->isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Unsafe payload path");
        return std::nullopt;
    }

    const auto sourceName = QString::fromStdString(sourcePath.string());
    QFile rawSource(sourceName);
    if (!rawSource.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = rawSource.errorString();
        return std::nullopt;
    }
    const auto magic = rawSource.peek(8);
    if (magic.startsWith(QByteArrayView{"\x7f" "ELF", 4})) {
        if (*safeTarget != QFileInfo(rawSource).fileName()) {
            if (error != nullptr) *error = QStringLiteral("Payload path does not name the ELF source file");
            return std::nullopt;
        }
        return inspectRawFile(rawSource, error);
    }
    rawSource.close();

    const auto encoded = QFile::encodeName(sourceName);
    if (!magic.startsWith(QByteArrayLiteral("!<arch>\n"))) {
        ArchivePointer artifact(archive_read_new());
        archive_read_support_filter_all(artifact.get());
        archive_read_support_format_all(artifact.get());
        if (archive_read_open_filename(artifact.get(), encoded.constData(), 64 * 1024) != ARCHIVE_OK) {
            if (error != nullptr) {
                *error = archiveError(artifact.get(), QStringLiteral("Could not open artifact archive"));
            }
            return std::nullopt;
        }
        return inspectArchiveEntry(artifact.get(), *safeTarget, error);
    }

    ArchivePointer outer(archive_read_new());
    archive_read_support_filter_all(outer.get());
    archive_read_support_format_ar(outer.get());
    if (archive_read_open_filename(outer.get(), encoded.constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error != nullptr) *error = archiveError(outer.get(), QStringLiteral("Could not open DEB"));
        return std::nullopt;
    }
    QTemporaryFile dataArchive;
    if (!dataArchive.open()) {
        if (error != nullptr) *error = dataArchive.errorString();
        return std::nullopt;
    }
    archive_entry *entry = nullptr;
    bool foundData = false;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(outer.get(), &entry)) == ARCHIVE_OK) {
        auto path = entryPath(entry);
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        if (path.startsWith(QStringLiteral("data.tar"))) {
            QString copyError;
            if (!copyEntry(outer.get(), dataArchive, copyError)) {
                if (error != nullptr) *error = copyError;
                return std::nullopt;
            }
            foundData = true;
            break;
        }
    }
    if (!foundData) {
        if (error != nullptr) *error = QStringLiteral("DEB has no data.tar.* member");
        return std::nullopt;
    }
    dataArchive.flush();

    ArchivePointer data(archive_read_new());
    archive_read_support_filter_all(data.get());
    archive_read_support_format_tar(data.get());
    const auto dataName = QFile::encodeName(dataArchive.fileName());
    if (archive_read_open_filename(data.get(), dataName.constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error != nullptr) *error = archiveError(data.get(), QStringLiteral("Could not open data archive"));
        return std::nullopt;
    }
    return inspectArchiveEntry(data.get(), *safeTarget, error);
}

std::optional<QByteArray> PayloadInspector::readFileBytes(
    const std::filesystem::path &sourcePath, const QString &payloadPath,
    const qsizetype maximumBytes, QString *error) {
    const auto safeTarget = PathSafety::normalizedArchivePath(payloadPath);
    if (!safeTarget || safeTarget->isEmpty() || maximumBytes <= 0) {
        if (error != nullptr) *error = QStringLiteral("Unsafe payload path or byte limit");
        return std::nullopt;
    }
    const auto sourceName = QString::fromStdString(sourcePath.string());
    QFile raw(sourceName);
    if (!raw.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = raw.errorString();
        return std::nullopt;
    }
    const auto magic = raw.peek(12);
    const bool appImage = magic.size() >= 11 &&
        magic.startsWith(QByteArrayView{"\x7f" "ELF", 4}) &&
        magic.at(8) == 'A' && magic.at(9) == 'I' && magic.at(10) == '\x02';
    if (appImage) {
        const auto offset = squashfsOffset(raw);
        const auto executable = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
        if (!offset || executable.isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("Static AppImage payload reading requires squashfs-tools");
            return std::nullopt;
        }
        QProcess process;
        process.setProgram(executable);
        process.setArguments({QStringLiteral("-cat"), QStringLiteral("-o"),
                              QString::number(*offset), sourceName, *safeTarget});
        process.start();
        if (!process.waitForStarted(5000)) {
            if (error != nullptr) *error = process.errorString();
            return std::nullopt;
        }
        QByteArray result;
        QElapsedTimer deadline;
        deadline.start();
        while (process.state() != QProcess::NotRunning) {
            process.waitForReadyRead(1000);
            result += process.readAllStandardOutput();
            if (result.size() > maximumBytes) {
                process.kill();
                process.waitForFinished(5000);
                if (error != nullptr) *error = QStringLiteral("Payload file exceeds the selection byte limit");
                return std::nullopt;
            }
            if (deadline.hasExpired(30000)) {
                process.kill();
                process.waitForFinished(5000);
                if (error != nullptr) {
                    *error = QStringLiteral("Static AppImage payload reading exceeded 30 seconds");
                }
                return std::nullopt;
            }
        }
        result += process.readAllStandardOutput();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (error != nullptr) *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
            return std::nullopt;
        }
        return result;
    }
    if (magic.startsWith(QByteArrayView{"\x7f" "ELF", 4})) {
        if (*safeTarget != QFileInfo(raw).fileName() || raw.size() > maximumBytes || !raw.seek(0)) {
            if (error != nullptr) *error = QStringLiteral("ELF payload does not match the selected path or exceeds the limit");
            return std::nullopt;
        }
        return raw.readAll();
    }
    raw.close();

    const auto encoded = QFile::encodeName(sourceName);
    if (magic != QByteArrayLiteral("!<arch>\n")) {
        ArchivePointer artifact(archive_read_new());
        archive_read_support_filter_all(artifact.get());
        archive_read_support_format_all(artifact.get());
        if (archive_read_open_filename(artifact.get(), encoded.constData(), 64 * 1024) != ARCHIVE_OK) {
            if (error != nullptr) *error = archiveError(artifact.get(), QStringLiteral("Could not open artifact archive"));
            return std::nullopt;
        }
        return readArchiveEntryBytes(artifact.get(), *safeTarget, maximumBytes, error);
    }

    ArchivePointer outer(archive_read_new());
    archive_read_support_filter_all(outer.get());
    archive_read_support_format_ar(outer.get());
    if (archive_read_open_filename(outer.get(), encoded.constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error != nullptr) *error = archiveError(outer.get(), QStringLiteral("Could not open DEB"));
        return std::nullopt;
    }
    QTemporaryFile dataArchive;
    if (!dataArchive.open()) {
        if (error != nullptr) *error = dataArchive.errorString();
        return std::nullopt;
    }
    archive_entry *entry = nullptr;
    bool found = false;
    while (archive_read_next_header(outer.get(), &entry) == ARCHIVE_OK) {
        auto path = entryPath(entry);
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        if (path.startsWith(QStringLiteral("data.tar"))) {
            QString copyError;
            if (!copyEntry(outer.get(), dataArchive, copyError)) {
                if (error != nullptr) *error = copyError;
                return std::nullopt;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        if (error != nullptr) *error = QStringLiteral("DEB has no data.tar.* member");
        return std::nullopt;
    }
    dataArchive.flush();
    ArchivePointer data(archive_read_new());
    archive_read_support_filter_all(data.get());
    archive_read_support_format_tar(data.get());
    const auto dataName = QFile::encodeName(dataArchive.fileName());
    if (archive_read_open_filename(data.get(), dataName.constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error != nullptr) *error = archiveError(data.get(), QStringLiteral("Could not open DEB data archive"));
        return std::nullopt;
    }
    return readArchiveEntryBytes(data.get(), *safeTarget, maximumBytes, error);
}

} // namespace pacsmith
