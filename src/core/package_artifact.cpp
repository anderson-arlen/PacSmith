#include "core/package_artifact.hpp"

#include "core/project_store.hpp"

#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>

#include <array>
#include <memory>

namespace pacsmith {
namespace {

struct ArchiveDeleter {
    void operator()(archive *value) const { archive_read_free(value); }
};

QString field(const QByteArray &pkginfo, const QByteArray &name) {
    const auto prefix = name + QByteArrayLiteral(" = ");
    for (const auto &line : pkginfo.split('\n')) {
        if (line.startsWith(prefix)) return QString::fromUtf8(line.sliced(prefix.size())).trimmed();
    }
    return {};
}

} // namespace

std::optional<PackageArtifact> PackageArtifactInspector::inspect(
    const std::filesystem::path &packagePath, const std::filesystem::path &releaseDirectory,
    QString *error) {
    const QFileInfo info(QString::fromUtf8(packagePath.string().c_str()));
    if (!info.isAbsolute() || !info.isFile() ||
        !info.fileName().contains(QStringLiteral(".pkg.tar.")) ||
        info.fileName().endsWith(QStringLiteral(".sig"))) {
        if (error != nullptr) *error = QStringLiteral("Not an absolute Arch package archive");
        return std::nullopt;
    }

    std::unique_ptr<archive, ArchiveDeleter> reader(archive_read_new());
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_tar(reader.get());
    if (archive_read_open_filename(reader.get(), packagePath.c_str(), 10240) != ARCHIVE_OK) {
        if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader.get()));
        return std::nullopt;
    }
    QByteArray pkginfo;
    archive_entry *entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
        const auto path = QString::fromUtf8(archive_entry_pathname(entry));
        if (path != QStringLiteral(".PKGINFO")) {
            archive_read_data_skip(reader.get());
            continue;
        }
        constexpr qsizetype maximumPkginfo = 1024 * 1024;
        std::array<char, 8192> buffer{};
        la_ssize_t count = 0;
        while ((count = archive_read_data(reader.get(), buffer.data(), buffer.size())) > 0) {
            if (pkginfo.size() + count > maximumPkginfo) {
                if (error != nullptr) *error = QStringLiteral(".PKGINFO exceeds the safety limit");
                return std::nullopt;
            }
            pkginfo.append(buffer.data(), count);
        }
        break;
    }
    const auto name = field(pkginfo, QByteArrayLiteral("pkgname"));
    const auto version = field(pkginfo, QByteArrayLiteral("pkgver"));
    if (name.isEmpty() || version.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Arch package does not contain usable .PKGINFO");
        return std::nullopt;
    }
    std::error_code filesystemError;
    auto relative = std::filesystem::relative(packagePath, releaseDirectory, filesystemError);
    if (filesystemError || relative.empty() || relative.native().starts_with("..")) {
        relative = packagePath.filename();
    }
    PackageArtifact result;
    result.relativePath = QString::fromUtf8(relative.generic_string().c_str());
    result.sha256 = sha256File(packagePath, error);
    if (result.sha256.isEmpty()) return std::nullopt;
    result.packageName = name;
    result.packageVersion = version;
    result.architecture = field(pkginfo, QByteArrayLiteral("arch"));
    result.size = info.size();
    result.createdAt = info.birthTime().isValid() ? info.birthTime() : info.lastModified();
    return result;
}

BuildRecord buildRecordFromResult(const QString &id, const BuildStatus status,
                                  const QString &log, const QStringList &packagePaths,
                                  const std::filesystem::path &releaseDirectory,
                                  const QDateTime &startedAt, const QDateTime &finishedAt) {
    BuildRecord result;
    result.id = id;
    result.status = status;
    result.log = log;
    result.startedAt = startedAt;
    result.finishedAt = finishedAt;
    for (const auto &path : packagePaths) {
        auto artifact = PackageArtifactInspector::inspect(
            std::filesystem::path(path.toUtf8().constData()), releaseDirectory, nullptr);
        if (artifact) result.artifacts.append(std::move(*artifact));
    }
    return result;
}

} // namespace pacsmith
