#include "core/managed_package.hpp"

#include <QUrl>

#include <alpm.h>
#include <alpm_list.h>

#include <algorithm>
#include <memory>

namespace pacsmith {
namespace {

struct HandleDeleter {
    void operator()(alpm_handle_t *handle) const {
        if (handle != nullptr) alpm_release(handle);
    }
};

QString decode(const QString &value) {
    return QUrl::fromPercentEncoding(value.toLatin1());
}

} // namespace

QString ManagedPackageInfo::projectId() const {
    return decode(xdata.value(QStringLiteral("pacsmith.project")));
}

QString ManagedPackageInfo::releaseId() const {
    return decode(xdata.value(QStringLiteral("pacsmith.release")));
}

QString ManagedPackageInfo::sourceIdentity() const {
    return decode(xdata.value(QStringLiteral("pacsmith.source")));
}

QList<ManagedPackageInfo> ManagedPackageRegistry::installed(QString *error) {
    alpm_errno_t code = ALPM_ERR_OK;
    std::unique_ptr<alpm_handle_t, HandleDeleter> handle(
        alpm_initialize("/", "/var/lib/pacman", &code));
    if (!handle) {
        if (error != nullptr) *error = QString::fromUtf8(alpm_strerror(code));
        return {};
    }
    auto *database = alpm_get_localdb(handle.get());
    if (database == nullptr) {
        if (error != nullptr) *error = QStringLiteral("libalpm could not open the local package database");
        return {};
    }
    QList<ManagedPackageInfo> result;
    for (auto *node = alpm_db_get_pkgcache(database); node != nullptr; node = alpm_list_next(node)) {
        auto *package = static_cast<alpm_pkg_t *>(node->data);
        ManagedPackageInfo info;
        info.packageName = QString::fromUtf8(alpm_pkg_get_name(package));
        info.packageVersion = QString::fromUtf8(alpm_pkg_get_version(package));
        for (auto *item = alpm_pkg_get_xdata(package); item != nullptr; item = alpm_list_next(item)) {
            const auto *data = static_cast<const alpm_pkg_xdata_t *>(item->data);
            if (data != nullptr && data->name != nullptr && data->value != nullptr) {
                info.xdata.insert(QString::fromUtf8(data->name), QString::fromUtf8(data->value));
            }
        }
        if (info.xdata.value(QStringLiteral("pacsmith.schema")) == QStringLiteral("1")) {
            result.append(std::move(info));
        }
    }
    return result;
}

std::optional<ManagedPackageInfo> ManagedPackageRegistry::find(const QString &packageName,
                                                               QString *error) {
    const auto packages = installed(error);
    const auto iterator = std::find_if(packages.cbegin(), packages.cend(), [&](const auto &candidate) {
        return candidate.packageName == packageName;
    });
    if (iterator == packages.cend()) return std::nullopt;
    return *iterator;
}

} // namespace pacsmith
