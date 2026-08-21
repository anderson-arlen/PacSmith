#include "core/apt_repository.hpp"

#include "core/control_parser.hpp"
#include "core/path_safety.hpp"

#include <QRegularExpression>
#include <QUrl>

#include <archive.h>
#include <archive_entry.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>

namespace pacsmith {
namespace {

struct ArchiveDeleter {
    void operator()(archive *value) const {
        if (value != nullptr) archive_read_free(value);
    }
};

QByteArray signedPayload(QByteArray data) {
    if (!data.startsWith("-----BEGIN PGP SIGNED MESSAGE-----")) return data;
    auto bodyStart = data.indexOf("\n\n");
    if (bodyStart < 0) return {};
    bodyStart += 2;
    const auto signature = data.indexOf("\n-----BEGIN PGP SIGNATURE-----", bodyStart);
    if (signature < 0) return {};
    data = data.mid(bodyStart, signature - bodyStart);
    data.replace("\n- ", "\n");
    if (data.startsWith("- ")) data.remove(0, 2);
    return data;
}

struct VersionParts {
    quint64 epoch{0};
    QByteArray upstream;
    QByteArray revision{"0"};
};

VersionParts splitVersion(const QString &version) {
    VersionParts result;
    QString remainder = version;
    const auto colon = remainder.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        bool valid = false;
        result.epoch = remainder.left(colon).toULongLong(&valid);
        if (!valid) result.epoch = 0;
        remainder = remainder.mid(colon + 1);
    }
    const auto dash = remainder.lastIndexOf(QLatin1Char('-'));
    if (dash >= 0) {
        result.upstream = remainder.left(dash).toUtf8();
        result.revision = remainder.mid(dash + 1).toUtf8();
    } else {
        result.upstream = remainder.toUtf8();
    }
    return result;
}

int characterOrder(const unsigned char value) {
    if (value == static_cast<unsigned char>('~')) return -1;
    if (value == 0U || std::isdigit(value) != 0) return 0;
    if (std::isalpha(value) != 0) return static_cast<int>(value);
    return static_cast<int>(value) + 256;
}

bool safeRepositoryFile(const QString &path) {
    const QUrl url(path);
    return url.isRelative() && !path.contains(QLatin1Char(':')) &&
           !path.contains(QLatin1Char('\\')) && !path.startsWith(QStringLiteral("//"));
}

int comparePart(const QByteArray &left, const QByteArray &right) {
    qsizetype leftIndex = 0;
    qsizetype rightIndex = 0;
    while (leftIndex < left.size() || rightIndex < right.size()) {
        while ((leftIndex < left.size() && std::isdigit(static_cast<unsigned char>(left.at(leftIndex))) == 0) ||
               (rightIndex < right.size() && std::isdigit(static_cast<unsigned char>(right.at(rightIndex))) == 0)) {
            const auto leftCharacter = leftIndex < left.size()
                                           ? static_cast<unsigned char>(left.at(leftIndex))
                                           : static_cast<unsigned char>(0);
            const auto rightCharacter = rightIndex < right.size()
                                            ? static_cast<unsigned char>(right.at(rightIndex))
                                            : static_cast<unsigned char>(0);
            const auto difference = characterOrder(leftCharacter) - characterOrder(rightCharacter);
            if (difference != 0) return difference < 0 ? -1 : 1;
            if (leftIndex < left.size()) ++leftIndex;
            if (rightIndex < right.size()) ++rightIndex;
        }

        while (leftIndex < left.size() && left.at(leftIndex) == '0') ++leftIndex;
        while (rightIndex < right.size() && right.at(rightIndex) == '0') ++rightIndex;
        const auto leftDigits = leftIndex;
        const auto rightDigits = rightIndex;
        while (leftIndex < left.size() && std::isdigit(static_cast<unsigned char>(left.at(leftIndex))) != 0) ++leftIndex;
        while (rightIndex < right.size() && std::isdigit(static_cast<unsigned char>(right.at(rightIndex))) != 0) ++rightIndex;
        const auto leftLength = leftIndex - leftDigits;
        const auto rightLength = rightIndex - rightDigits;
        if (leftLength != rightLength) return leftLength < rightLength ? -1 : 1;
        const auto comparison = QByteArrayView(left).sliced(leftDigits, leftLength)
                                    .compare(QByteArrayView(right).sliced(rightDigits, rightLength));
        if (comparison != 0) return comparison < 0 ? -1 : 1;
    }
    return 0;
}

} // namespace

int DebianVersion::compare(const QString &left, const QString &right) {
    const auto leftParts = splitVersion(left);
    const auto rightParts = splitVersion(right);
    if (leftParts.epoch != rightParts.epoch) return leftParts.epoch < rightParts.epoch ? -1 : 1;
    const auto upstream = comparePart(leftParts.upstream, rightParts.upstream);
    return upstream != 0 ? upstream : comparePart(leftParts.revision, rightParts.revision);
}

QList<AptReleaseEntry> AptRepositoryMetadata::parseRelease(const QByteArrayView data, QString *error) {
    const auto payload = signedPayload(QByteArray(data.data(), data.size()));
    if (payload.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Invalid clear-signed InRelease data");
        return {};
    }
    const auto paragraphs = ControlParser::parseParagraphs(QByteArrayView(payload));
    if (paragraphs.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Release metadata has no fields");
        return {};
    }
    const auto shaField = paragraphs.first().value(QStringLiteral("SHA256"));
    static const QRegularExpression lineExpression(
        QStringLiteral(R"(^([0-9a-fA-F]{64})\s+([0-9]+)\s+(.+)$)"));
    QList<AptReleaseEntry> result;
    for (const auto &line : shaField.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto match = lineExpression.match(line.trimmed());
        if (!match.hasMatch()) continue;
        const auto safePath = PathSafety::normalizedArchivePath(match.captured(3));
        bool sizeValid = false;
        const auto size = match.captured(2).toLongLong(&sizeValid);
        if (!safePath || safePath->isEmpty() || !safeRepositoryFile(*safePath) || !sizeValid || size < 0) continue;
        result.append({*safePath, match.captured(1).toLower(), size});
    }
    if (result.isEmpty() && error != nullptr) {
        *error = QStringLiteral("Release metadata has no usable SHA256 index entries");
    }
    return result;
}

std::optional<AptReleaseEntry> AptRepositoryMetadata::selectPackagesIndex(
    const QList<AptReleaseEntry> &entries, const QString &component,
    const QString &architecture, const bool flatRepository) {
    const auto stem = flatRepository ? QStringLiteral("Packages")
                                     : QStringLiteral("%1/binary-%2/Packages").arg(component, architecture);
    const QStringList suffixes{QStringLiteral(".xz"), QStringLiteral(".gz"),
                               QStringLiteral(".zst"), QString{}};
    for (const auto &suffix : suffixes) {
        const auto wanted = stem + suffix;
        const auto iterator = std::find_if(entries.cbegin(), entries.cend(),
                                           [&wanted](const auto &entry) { return entry.path == wanted; });
        if (iterator != entries.cend()) return *iterator;
    }
    return std::nullopt;
}

std::optional<QByteArray> AptRepositoryMetadata::decompressIndex(const QByteArray &data, QString *error) {
    constexpr qsizetype maximumSize = 256 * 1024 * 1024;
    std::unique_ptr<archive, ArchiveDeleter> reader(archive_read_new());
    if (!reader) {
        if (error != nullptr) *error = QStringLiteral("Could not allocate index decompressor");
        return std::nullopt;
    }
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_raw(reader.get());
    if (archive_read_open_memory(reader.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK) {
        if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader.get()));
        return std::nullopt;
    }
    archive_entry *entry = nullptr;
    if (archive_read_next_header(reader.get(), &entry) != ARCHIVE_OK) {
        if (error != nullptr) *error = QStringLiteral("Could not open Packages index stream");
        return std::nullopt;
    }
    QByteArray output;
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader.get()));
            return std::nullopt;
        }
        const auto length = static_cast<qsizetype>(count);
        if (output.size() > maximumSize - length) {
            if (error != nullptr) *error = QStringLiteral("Decompressed Packages index exceeds 256 MiB safety limit");
            return std::nullopt;
        }
        output.append(buffer.data(), length);
    }
    return output;
}

std::optional<AptPackageRecord> AptRepositoryMetadata::latestPackage(
    const QByteArrayView packages, const QString &packageName,
    const QString &architecture, QString *error) {
    std::optional<AptPackageRecord> best;
    for (const auto &fields : ControlParser::parseParagraphs(packages)) {
        if (fields.value(QStringLiteral("Package")) != packageName) continue;
        const auto candidateArchitecture = fields.value(QStringLiteral("Architecture"));
        if (candidateArchitecture != architecture && candidateArchitecture != QStringLiteral("all")) continue;
        AptPackageRecord candidate{packageName, fields.value(QStringLiteral("Version")), candidateArchitecture,
                                   fields.value(QStringLiteral("Filename")),
                                   fields.value(QStringLiteral("SHA256"))};
        bool validSize = false;
        candidate.size = fields.value(QStringLiteral("Size")).toLongLong(&validSize);
        const auto safeFilename = PathSafety::normalizedArchivePath(candidate.filename);
        static const QRegularExpression shaExpression(QStringLiteral(R"(^[0-9a-fA-F]{64}$)"));
        if (candidate.version.isEmpty() || !safeFilename || safeFilename->isEmpty() ||
            !safeRepositoryFile(*safeFilename) ||
            !shaExpression.match(candidate.sha256).hasMatch() || !validSize || candidate.size < 0) continue;
        candidate.filename = *safeFilename;
        candidate.sha256 = candidate.sha256.toLower();
        if (!best || DebianVersion::compare(candidate.version, best->version) > 0) best = candidate;
    }
    if (!best && error != nullptr) {
        *error = QStringLiteral("Package %1 for architecture %2 was not found in the repository index")
                     .arg(packageName, architecture);
    }
    return best;
}

} // namespace pacsmith
