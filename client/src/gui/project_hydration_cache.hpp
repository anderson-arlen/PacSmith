#pragma once

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

namespace pacsmith::gui {

class ProjectHydrationCache final {
public:
    static constexpr qint64 freshnessMilliseconds = 5 * 60 * 1000;

    void markLoaded(const QString &projectId,
                    const QDateTime &loadedAt = QDateTime::currentDateTimeUtc()) {
        if (!projectId.isEmpty()) loadedAt_.insert(projectId, loadedAt);
    }

    [[nodiscard]] bool contains(const QString &projectId) const {
        return loadedAt_.contains(projectId);
    }

    [[nodiscard]] bool isFresh(
        const QString &projectId,
        const QDateTime &now = QDateTime::currentDateTimeUtc()) const {
        const auto found = loadedAt_.constFind(projectId);
        if (found == loadedAt_.cend() || !found->isValid() || !now.isValid()) return false;
        const auto age = found->msecsTo(now);
        return age >= 0 && age < freshnessMilliseconds;
    }

    void remove(const QString &projectId) { loadedAt_.remove(projectId); }
    void clear() { loadedAt_.clear(); }

    void retain(const QSet<QString> &projectIds) {
        for (auto iterator = loadedAt_.begin(); iterator != loadedAt_.end();) {
            if (projectIds.contains(iterator.key())) ++iterator;
            else iterator = loadedAt_.erase(iterator);
        }
    }

private:
    QHash<QString, QDateTime> loadedAt_;
};

} // namespace pacsmith::gui
