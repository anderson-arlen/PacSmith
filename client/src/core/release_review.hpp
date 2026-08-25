#pragma once

#include "core/model.hpp"

#include <QList>
#include <QString>

namespace pacsmith {

struct ReleaseReviewIssue {
    QString code;
    QString category;
    QString summary;
    QString subject;
    QString remediation;
    bool blocksBuild{false};
};

[[nodiscard]] QList<ReleaseReviewIssue> releaseReviewIssues(const PackageRelease &release);
[[nodiscard]] bool archiveDesktopCommandUnmapped(const PackageRelease &release);

} // namespace pacsmith
