#pragma once

#include "core/model.hpp"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace pacsmith {

struct ReleaseReviewIssue {
    QString code;
    QString category;
    QString summary;
    QString subject;
    QString remediation;
    bool blocksBuild{false};
};

struct AutomaticUpdateBuildSelection {
    QString previousReleaseId;
    QString preparedReleaseId;
};

[[nodiscard]] QList<ReleaseReviewIssue> releaseReviewIssues(const PackageRelease &release);
[[nodiscard]] bool archiveDesktopCommandUnmapped(const PackageRelease &release);
[[nodiscard]] QStringList automaticUpdateBuildBlockers(
    const PackageRelease &previous, const PackageRelease &next);
[[nodiscard]] std::optional<AutomaticUpdateBuildSelection>
automaticUpdateBuildSelection(const Project &project);

} // namespace pacsmith
